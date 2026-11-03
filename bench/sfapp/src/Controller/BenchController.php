<?php
namespace App\Controller;

use App\Dto;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\Routing\Attribute\Route;
use Symfony\Component\Serializer\SerializerInterface;
use Symfony\Component\Validator\Validator\ValidatorInterface;
use Symfony\Component\DependencyInjection\Attribute\Autowire;
use Symfony\Contracts\Cache\CacheInterface;

class BenchController
{
    private CacheInterface $cache;

    public function __construct(CacheInterface $cache, #[Autowire(service: 'app.cache_raw')] CacheInterface $raw)
    {
        $this->cache = getenv('RAW') ? $raw : $cache; // RAW=1: bypass Symfony's serializing marshaller
    }

    private function catalog(): array
    {
        return $this->cache->get('catalog.v1', function () {
            mt_srand(1);
            $rows = [];
            for ($i = 1; $i <= 2000; $i++) {
                $rows[$i] = ['id' => $i, 'sku' => "SKU-$i", 'name' => "Product number $i", 'price' => mt_rand(100, 99900) / 100,
                    'stock' => mt_rand(0, 500), 'tags' => ['t' . ($i % 7), 't' . ($i % 13), 'new'],
                    'attrs' => ['color' => ['red', 'green', 'blue'][$i % 3], 'size' => ['S', 'M', 'L'][$i % 3], 'weight' => $i % 50]];
            }
            return $rows;
        });
    }

    #[Route('/hello')]
    public function hello(): Response
    {
        return new Response('ok');
    }

    #[Route('/products')]
    public function products(Request $req): JsonResponse
    {
        $page = (int) $req->query->get('page', 1);
        return new JsonResponse(array_slice($this->catalog(), ($page - 1) * 20, 20, true));
    }

    #[Route('/product/{id}')]
    public function product(int $id): JsonResponse
    {
        return new JsonResponse($this->catalog()[$id] ?? null);
    }

    #[Route('/validate')]
    public function validate(SerializerInterface $serializer, ValidatorInterface $validator): JsonResponse
    {
        $dto = $serializer->deserialize('{"name":"Ada Lovelace","email":"ada@example.com","age":36,"tags":["a","b"]}', Dto::class, 'json');
        $errors = $validator->validate($dto);
        return new JsonResponse(['n' => count($errors), 'dto' => json_decode($serializer->serialize($dto, 'json'))]);
    }
}
