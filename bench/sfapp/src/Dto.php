<?php
namespace App;

use Symfony\Component\Validator\Constraints as Assert;

class Dto
{
    #[Assert\NotBlank] #[Assert\Length(min: 3, max: 40)]
    public string $name = '';
    #[Assert\Email]
    public string $email = '';
    #[Assert\Range(min: 1, max: 120)]
    public int $age = 0;
    #[Assert\Count(min: 1)]
    public array $tags = [];
}
